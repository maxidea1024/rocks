#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include "CurveUeReference.h"

// UE 5.8 FRichCurve::CompressCurve(형식 결정·키 시간 형식 판정·byte 배치)를 그대로 옮긴 시험 참조다
// (Docs/Design/CurveData/README.ko.md 11절·16절 — PackedTangentCurveTests.cpp가 Rocks
// PackedTangentCurve와 byte 단위로 대조한다). UE의 TArray는 Array, 포인터 쓰기는 같은 위치의 byte
// 쓰기(리틀 엔디언 — UE Windows x64 메모리 배치와 같다)로 옮겼다. 두 키 사이 식은 Core
// KeyCurveUeReference.h (ue58curve)를 쓴다.
//
// 참조 함수 ↔ UE 5.8.4 원본(Engine/Source/Runtime/Engine/Private/Curves/RichCurve.cpp, 리뷰어 줄
// 단위 대조용):
//   ue58pack::FindCompressionFormat ↔ RichCurve.cpp 1279–1330(IsConstant 485–502, 허용오차
//                                     UE_SMALL_NUMBER — RichCurve.h 277)
//   ue58pack::FindKeyFormat         ↔ RichCurve.cpp 1332–1475
//   ue58pack::CompressCurve         ↔ RichCurve.cpp 1477–1682(MIXEDKEY_STRIPS_TANGENTS 0 갈래 —
//                                     RichCurve.cpp 10)
//   ue58pack::RoundHalfFromZero     ↔ UnrealMath.cpp 3033–3046
//   ue58pack::Clamp                 ↔ UnrealMathUtility.h 592–595(NaN이면 Max)
// UE 결함 하나는 고쳐 옮겼다(설계 15절 32): FindKeyFormat의 손실 키 전진 뒤 범위 검사가
// `>= NumKeys`라 복원한 마지막 키 시간이 max보다 작으면 Keys[NumKeys]를 읽는다(UE도 배열 밖
// 읽기, check 빌드는 assert). 참조는 `CurrentLossyKey + 1 >= NumKeys`로 막는다.
namespace rocks::test::ue58pack {

using ue58richcurve::Interp;
using ue58richcurve::RichKey;
using ue58richcurve::Weight;

// UE ERichCurveCompressionFormat·ERichCurveKeyTimeCompressionFormat 번호다.
enum class Format : uint8 {
    Empty = 0,
    Constant = 1,
    Linear = 2,
    Cubic = 3,
    Mixed = 4,
    Weighted = 5
};

enum class KeyFormat : uint8 {
    Uint16 = 0,
    Float32 = 1
};

// FCompressedRichCurve의 칸이다(ConstantValueNumKeys union은 두 칸으로 나눴다).
struct Compressed {
    explicit Compressed(Allocator& allocator) noexcept : CompressedKeys(allocator) {
    }

    Format CompressionFormat = Format::Empty;
    KeyFormat KeyTimeCompressionFormat = KeyFormat::Float32;
    ue58richcurve::Extrap PreInfinityExtrap = ue58richcurve::Extrap::Constant;
    ue58richcurve::Extrap PostInfinityExtrap = ue58richcurve::Extrap::Constant;
    float ConstantValue = 0.0f;
    int32 NumKeys = 0;
    Array<uint8> CompressedKeys;
};

constexpr float UE_SMALL_NUMBER = 1.e-8f;

inline int32 Align(int32 Value, int32 Alignment) noexcept {
    return (Value + Alignment - 1) & ~(Alignment - 1);
}

// UnrealMathUtility.h 592–595: Max(Min(X, MaxValue), MinValue) — Min·Max는 GenericPlatformMath의
// `A < B ? A : B`·`B < A ? A : B`라 X가 NaN이면 MaxValue다.
inline float Clamp(float X, float MinValue, float MaxValue) noexcept {
    const float Lower = (X < MaxValue) ? X : MaxValue;
    return (MinValue < Lower) ? Lower : MinValue;
}

// UnrealMath.cpp 3033–3046(Modf = 0 쪽으로 자른 정수부와 나머지).
inline float RoundHalfFromZero(float F) noexcept {
    const float ValueToFudgeIntegralPart =
        F < 0.0f ? -platform::PlatformMath::Floor(-F) : platform::PlatformMath::Floor(F);
    const float ValueToFudgeFractionalPart = F - ValueToFudgeIntegralPart;

    if (F < 0.0f) {
        return ValueToFudgeFractionalPart > -0.5f ? ValueToFudgeIntegralPart
                                                  : ValueToFudgeIntegralPart - 1.0f;
    } else {
        return ValueToFudgeFractionalPart < 0.5f ? ValueToFudgeIntegralPart
                                                 : ValueToFudgeIntegralPart + 1.0f;
    }
}

// RichCurve.cpp 1279–1330. Rocks에 RCIM_None이 없으므로 그 갈래(RCCF_Constant)는 빠진다.
inline Format FindCompressionFormat(const Array<RichKey>& Keys) noexcept {
    if (Keys.Size() == 0) {
        return Format::Empty;
    }

    if (ue58richcurve::IsConstant(Keys, UE_SMALL_NUMBER)) {
        return Format::Constant;
    }

    const RichKey& RefKey = Keys[0];
    bool isMixed = false;
    bool isWeighted = false;
    for (const RichKey& Key : Keys.View()) {
        if (Key.interp == Interp::Cubic) {
            if (Key.tangentWeight != Weight::None) {
                isWeighted = true;
                break;
            }
        }
        if (Key.interp != RefKey.interp) {
            isMixed = true;
        }
    }
    if (isWeighted) {
        return Format::Weighted;
    } else if (isMixed) {
        return Format::Mixed;
    }
    switch (RefKey.interp) {
    case Interp::Constant:
        return Format::Mixed;
    case Interp::Linear:
        return Format::Linear;
    case Interp::Cubic:
    default:
        return Format::Cubic;
    }
}

// RichCurve.cpp 1332–1475(표본 시간 float 누적·참조·손실 키 한 칸씩 전진 — UE 그대로).
inline KeyFormat FindKeyFormat(const Array<RichKey>& Keys, float ErrorThreshold, float SampleRate,
                               Format CompressionFormat,
                               const ue58curve::Options& options) noexcept {
    const int32 NumKeys = static_cast<int32>(Keys.Size());
    if (NumKeys == 0 || CompressionFormat == Format::Constant ||
        CompressionFormat == Format::Empty || ErrorThreshold <= 0.0f || SampleRate <= 0.0f) {
        return KeyFormat::Float32;
    }

    auto EvalForTwoKeys = [&options](const RichKey& Key1, float KeyTime1, const RichKey& Key2,
                                     float KeyTime2, float InTime) noexcept {
        const float Diff = KeyTime2 - KeyTime1;

        if (Diff > 0.f && Key1.interp != Interp::Constant) {
            const float Alpha = (InTime - KeyTime1) / Diff;
            const float P0 = Key1.value;
            const float P3 = Key2.value;

            if (Key1.interp == Interp::Linear) {
                return ue58curve::Lerp(P0, P3, Alpha, options);
            } else if (ue58curve::IsItNotWeighted(Key1, Key2)) {
                const float OneThird = 1.0f / 3.0f;
                const float P1 = P0 + (Key1.leaveTangent * Diff * OneThird);
                const float P2 = P3 - (Key2.arriveTangent * Diff * OneThird);

                return ue58curve::BezierInterp(P0, P1, P2, P3, Alpha, options);
            } else {
                return ue58curve::WeightedEvalForTwoKeys(
                    Key1.value, KeyTime1, Key1.leaveTangent, Key1.leaveTangentWeight,
                    Key1.tangentWeight, Key2.value, KeyTime2, Key2.arriveTangent,
                    Key2.arriveTangentWeight, Key2.tangentWeight, InTime, options);
            }
        } else {
            return Key1.value;
        }
    };

    auto DecayTime = [](const RichKey& Key, float MinTime, float DeltaTime, float InvDeltaTime,
                        float QuantizationScale, float InvQuantizationScale) noexcept {
        const float NormalizedTime = Clamp((Key.time - MinTime) * InvDeltaTime, 0.0f, 1.0f);
        const float QuantizedTime = RoundHalfFromZero(NormalizedTime * QuantizationScale);
        const float LossyNormalizedTime = QuantizedTime * InvQuantizationScale;
        return (LossyNormalizedTime * DeltaTime) + MinTime;
    };

    const float MinTime = Keys[0].time;
    const float MaxTime = Keys[NumKeys - 1].time;
    const float DeltaTime = MaxTime - MinTime;
    const float InvDeltaTime = 1.0f / DeltaTime;
    const float SampleRateIncrement = 1.0f / SampleRate;

    bool fitsOn16Bits = true;

    int32 CurrentLossyKey = 0;
    int32 CurrentRefKey = 0;
    float CurrentTime = MinTime;
    while (CurrentTime <= MaxTime && fitsOn16Bits) {
        if (CurrentTime > Keys[CurrentRefKey + 1].time) {
            CurrentRefKey++;

            if (CurrentRefKey >= NumKeys) {
                break;
            }
        }

        float LossyTime1_16;
        float LossyTime2_16 = DecayTime(Keys[CurrentLossyKey + 1], MinTime, DeltaTime, InvDeltaTime,
                                        65535.0f, 1.0f / 65535.0f);
        if (CurrentTime > LossyTime2_16) {
            CurrentLossyKey++;

            // UE 결함 수정(15절 32): UE는 `CurrentLossyKey >= NumKeys`라 마지막 키에서
            // Keys[NumKeys]를 읽는다(RC 1413–1421 — 복원한 마지막 시간이 max보다 1 ulp 작을 때).
            if (CurrentLossyKey + 1 >= NumKeys) {
                break;
            }

            LossyTime1_16 = LossyTime2_16;
            LossyTime2_16 = DecayTime(Keys[CurrentLossyKey + 1], MinTime, DeltaTime, InvDeltaTime,
                                      65535.0f, 1.0f / 65535.0f);
        } else {
            LossyTime1_16 = DecayTime(Keys[CurrentLossyKey], MinTime, DeltaTime, InvDeltaTime,
                                      65535.0f, 1.0f / 65535.0f);
        }

        const float Result_16 =
            EvalForTwoKeys(Keys[CurrentLossyKey], LossyTime1_16, Keys[CurrentLossyKey + 1],
                           LossyTime2_16, CurrentTime);
        const float Result_Ref = ue58curve::EvalForTwoKeys(
            Keys[CurrentRefKey], Keys[CurrentRefKey + 1], CurrentTime, options);

        const float Error_16 = platform::PlatformMath::Abs(Result_Ref - Result_16);

        fitsOn16Bits &= Error_16 <= ErrorThreshold;

        CurrentTime += SampleRateIncrement;
    }

    int32 SizeInterpMode = 0;
    if (CompressionFormat == Format::Mixed) {
        SizeInterpMode += NumKeys * 1;
    } else if (CompressionFormat == Format::Weighted) {
        SizeInterpMode += NumKeys * 2 * 1;
    }

    const int32 SizeUInt16 = Align(Align(SizeInterpMode, 2) + (NumKeys * 2), 4) + (2 * 4);
    const int32 SizeFloat32 = Align(SizeInterpMode, 4) + NumKeys * 4;

    if (fitsOn16Bits && SizeUInt16 < SizeFloat32) {
        return KeyFormat::Uint16;
    } else {
        return KeyFormat::Float32;
    }
}

inline void WriteUint16(Array<uint8>& Bytes, int32 Offset, uint16 Value) noexcept {
    Bytes[static_cast<usize>(Offset)] = static_cast<uint8>(Value & 0xFF);
    Bytes[static_cast<usize>(Offset) + 1] = static_cast<uint8>(Value >> 8);
}

inline void WriteFloat(Array<uint8>& Bytes, int32 Offset, float Value) noexcept {
    const uint32 Bits = platform::BitCast<uint32>(Value);
    for (usize Byte = 0; Byte < 4; ++Byte) {
        Bytes[static_cast<usize>(Offset) + Byte] = static_cast<uint8>((Bits >> (8 * Byte)) & 0xFF);
    }
}

inline uint8 InterpCode(Interp Mode) noexcept {
    if (Mode == Interp::Linear) {
        return static_cast<uint8>(Format::Linear);
    } else if (Mode == Interp::Cubic) {
        return static_cast<uint8>(Format::Cubic);
    } else {
        return static_cast<uint8>(Format::Constant);
    }
}

// RichCurve.cpp 1477–1682. FindCompressionFormat·FindKeyFormat 대신 정해 둔 형식을 받을 수 있다
// (Rocks가 UE와 다르게 정하는 사례에서 같은 형식의 배치만 대조하려고 —
// forcedFormat·forcedKeyFormat).
inline void CompressCurve(const Array<RichKey>& Keys, const ue58richcurve::CurveState& State,
                          float ErrorThreshold, float SampleRate, Compressed& OutCurve,
                          const ue58curve::Options& options, const Format* forcedFormat = nullptr,
                          const KeyFormat* forcedKeyFormat = nullptr) noexcept {
    const Format CompressionFormat = forcedFormat ? *forcedFormat : FindCompressionFormat(Keys);
    OutCurve.CompressionFormat = CompressionFormat;

    const KeyFormat KeyFormatValue =
        forcedKeyFormat
            ? *forcedKeyFormat
            : FindKeyFormat(Keys, ErrorThreshold, SampleRate, CompressionFormat, options);
    OutCurve.KeyTimeCompressionFormat = KeyFormatValue;

    OutCurve.PreInfinityExtrap = State.PreInfinityExtrap;
    OutCurve.PostInfinityExtrap = State.PostInfinityExtrap;
    OutCurve.CompressedKeys.Clear();
    OutCurve.NumKeys = 0;

    const int32 Num = static_cast<int32>(Keys.Size());
    if (CompressionFormat == Format::Empty) {
        OutCurve.ConstantValue = State.DefaultValue;
    } else if (CompressionFormat == Format::Constant) {
        OutCurve.ConstantValue = Keys[0].value;
    } else {
        int32 PackedDataSize = 0;

        if (CompressionFormat == Format::Mixed) {
            PackedDataSize += Num * 1;
        } else if (CompressionFormat == Format::Weighted) {
            PackedDataSize += Num * 2 * 1;
        }

        if (KeyFormatValue == KeyFormat::Uint16) {
            PackedDataSize = Align(PackedDataSize, 2);
            PackedDataSize += Num * 2;
            PackedDataSize = Align(PackedDataSize, 4);
            PackedDataSize += 2 * 4;
        } else {
            PackedDataSize = Align(PackedDataSize, 4);
            PackedDataSize += Num * 4;
        }

        PackedDataSize += Num * 4; // Key values

        if (CompressionFormat == Format::Cubic) {
            PackedDataSize += Num * 2 * 4;
        } else if (CompressionFormat == Format::Mixed) {
            PackedDataSize += Num * 2 * 4; // Always have tangents
        } else if (CompressionFormat == Format::Weighted) {
            PackedDataSize += Num * 4 * 4;
        }

        // AddZeroed
        static_cast<void>(OutCurve.CompressedKeys.TryResize(static_cast<usize>(PackedDataSize)));
        for (usize Byte = 0; Byte < OutCurve.CompressedKeys.Size(); ++Byte) {
            OutCurve.CompressedKeys[Byte] = 0;
        }

        int32 WriteOffset = 0;
        OutCurve.NumKeys = Num;

        if (CompressionFormat == Format::Mixed) {
            int32 InterpModes = WriteOffset;
            WriteOffset += Num * 1;

            for (const RichKey& Key : Keys.View()) {
                OutCurve.CompressedKeys[static_cast<usize>(InterpModes++)] = InterpCode(Key.interp);
            }
        } else if (CompressionFormat == Format::Weighted) {
            int32 InterpModes = WriteOffset;
            WriteOffset += Num * 2 * 1;

            for (const RichKey& Key : Keys.View()) {
                OutCurve.CompressedKeys[static_cast<usize>(InterpModes++)] = InterpCode(Key.interp);
                OutCurve.CompressedKeys[static_cast<usize>(InterpModes++)] =
                    static_cast<uint8>(Key.tangentWeight);
            }
        }

        if (KeyFormatValue == KeyFormat::Uint16) {
            const float MinTime = Keys[0].time;
            const float MaxTime = Keys[static_cast<usize>(Num - 1)].time;
            const float DeltaTime = MaxTime - MinTime;
            const float InvDeltaTime = 1.0f / DeltaTime;

            WriteOffset = Align(WriteOffset, 2);
            int32 KeyTimes16 = WriteOffset;
            WriteOffset += Num * 2;

            for (const RichKey& Key : Keys.View()) {
                const float NormalizedTime = Clamp((Key.time - MinTime) * InvDeltaTime, 0.0f, 1.0f);
                const uint16 QuantizedTime =
                    static_cast<uint16>(RoundHalfFromZero(NormalizedTime * 65535.0f));
                WriteUint16(OutCurve.CompressedKeys, KeyTimes16, QuantizedTime);
                KeyTimes16 += 2;
            }

            WriteOffset = Align(WriteOffset, 4);
            const int32 RangeData = WriteOffset;
            WriteOffset += 2 * 4;

            WriteFloat(OutCurve.CompressedKeys, RangeData, MinTime);
            WriteFloat(OutCurve.CompressedKeys, RangeData + 4, DeltaTime);
        } else {
            WriteOffset = Align(WriteOffset, 4);
            int32 KeyTimes = WriteOffset;
            WriteOffset += Num * 4;

            for (const RichKey& Key : Keys.View()) {
                WriteFloat(OutCurve.CompressedKeys, KeyTimes, Key.time);
                KeyTimes += 4;
            }
        }

        int32 KeyData = WriteOffset;
        for (const RichKey& Key : Keys.View()) {
            WriteFloat(OutCurve.CompressedKeys, KeyData, Key.value);
            KeyData += 4;

            if (CompressionFormat == Format::Mixed || Key.interp == Interp::Cubic ||
                CompressionFormat == Format::Weighted) {
                WriteFloat(OutCurve.CompressedKeys, KeyData, Key.arriveTangent);
                WriteFloat(OutCurve.CompressedKeys, KeyData + 4, Key.leaveTangent);
                KeyData += 8;
            }
            if (CompressionFormat == Format::Weighted) {
                WriteFloat(OutCurve.CompressedKeys, KeyData, Key.arriveTangentWeight);
                WriteFloat(OutCurve.CompressedKeys, KeyData + 4, Key.leaveTangentWeight);
                KeyData += 8;
            }
        }
    }
}

} // namespace rocks::test::ue58pack
